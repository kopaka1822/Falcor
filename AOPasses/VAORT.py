from falcor import *

def render_graph_VAORT():
    g = RenderGraph('VAORT')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    VAORT = createPass('VAORT')
    g.addPass(VAORT, 'VAORT')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    CrossBilateralBlur = createPass('CrossBilateralBlur')
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    ConvertFormat = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat, 'ConvertFormat')
    ConvertFormat0 = createPass('ConvertFormat', {'formula': 'I0[xy].x * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat0, 'ConvertFormat0')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.faceNormalW', 'VAORT.normals')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('LinearizeDepth.linearDepth', 'VAORT.depth')
    g.addEdge('VAORT.ambientMap', 'CrossBilateralBlur.color')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat.I0')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat0.I0')
    g.addEdge('GBufferRaster.diffuseOpacity', 'ConvertFormat0.I1')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.markOutput('ConvertFormat.out')
    g.markOutput('ConvertFormat0.out')
    return g

VAORT = render_graph_VAORT()
try: m.addGraph(VAORT)
except NameError: None
