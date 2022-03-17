from falcor import *

def render_graph_RTAO():
    g = RenderGraph('RTAO')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('VAONonInterleaved.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    RTAO = createPass('RTAO')
    g.addPass(RTAO, 'RTAO')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    RTAODenoiser = createPass('RTAODenoiser')
    g.addPass(RTAODenoiser, 'RTAODenoiser')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.depth', 'RTAO.depth')
    g.addEdge('GBufferRaster.normW', 'RTAO.normals')
    g.addEdge('GBufferRaster.faceNormalW', 'RTAO.faceNormal')
    g.addEdge('RTAO.ambient', 'Ambient.I0')
    g.addEdge('RTAO.ambient', 'Diffuse.I0')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('RTAO.ambient', 'RTAODenoiser.aoImage')
    g.addEdge('GBufferRaster.normW', 'RTAODenoiser.normal')
    g.addEdge('GBufferRaster.depth', 'RTAODenoiser.depth')
    g.addEdge('GBufferRaster.linearZ', 'RTAODenoiser.linearDepth')
    g.addEdge('GBufferRaster.mvec', 'RTAODenoiser.mVec')
    g.addEdge('RTAODenoiser.denoisedOut', 'Diffuse.I2')
    g.addEdge('RTAO.rayDistance', 'RTAODenoiser.rayDistance')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g

RTAO = render_graph_RTAO()
try: m.addGraph(RTAO)
except NameError: None
