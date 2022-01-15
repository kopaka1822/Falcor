from falcor import *

def render_graph_SSAO():
    g = RenderGraph('SSAO')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SSAO = createPass('SSAO', {'enabled': True, 'halfResolution': False, 'kernelSize': 16, 'noiseSize': uint2(16,16), 'radius': 2.0, 'distribution': SampleDistribution.UniformHammersley, 'blurWidth': 5, 'blurSigma': 2.0})
    g.addPass(SSAO, 'SSAO')
    g.addEdge('GBufferRaster.depth', 'SSAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.markOutput('SSAO.ambientMap')
    return g

RenderGraph = render_graph_SSAO()
try: m.addGraph(RenderGraph)
except NameError: None