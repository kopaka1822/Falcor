# Graphs
from falcor import *

def render_graph_BillboardRenderGraph():
    g = RenderGraph('BillboardRenderGraph')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ConstantColor.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('BillboardRayTracer.dll')
    GBufferRT = createPass('GBufferRT', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'disableAlphaTest': False, 'forceCullMode': False, 'cull': CullMode.CullBack, 'texLOD': LODMode.UseMip0})
    g.addPass(GBufferRT, 'GBufferRT')
    ToneMapper = createPass('ToneMapper', {'exposureCompensation': 0.0, 'autoExposure': False, 'exposureValue': 0.0, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapper, 'ToneMapper')
    BillboardRayTracer = createPass('BillboardRayTracer', {'mMaxBounces': 3, 'mComputeDirect': True})
    g.addPass(BillboardRayTracer, 'BillboardRayTracer')
    g.addEdge('BillboardRayTracer.color', 'ToneMapper.src')
    g.addEdge('GBufferRT.posW', 'BillboardRayTracer.posW')
    g.addEdge('GBufferRT.normW', 'BillboardRayTracer.normalW')
    g.addEdge('GBufferRT.tangentW', 'BillboardRayTracer.tangentW')
    g.addEdge('GBufferRT.diffuseOpacity', 'BillboardRayTracer.mtlDiffOpacity')
    g.addEdge('GBufferRT.specRough', 'BillboardRayTracer.mtlSpecRough')
    g.addEdge('GBufferRT.emissive', 'BillboardRayTracer.mtlEmissive')
    g.addEdge('GBufferRT.matlExtra', 'BillboardRayTracer.mtlParams')
    g.addEdge('GBufferRT.faceNormalW', 'BillboardRayTracer.faceNormalW')
    g.addEdge('GBufferRT.viewW', 'BillboardRayTracer.viewW')
    g.markOutput('ToneMapper.dst')
    return g
m.addGraph(render_graph_BillboardRenderGraph())

# Scene
m.loadScene('Arcade/Arcade.fscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True)
m.scene.camera.position = float3(-1.143306,1.843090,2.442334)
m.scene.camera.target = float3(-0.701423,1.486366,1.619238)
m.scene.camera.up = float3(-0.376237,0.634521,0.675103)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(1920, 1061)
m.ui = True

# Time Settings
t.time = 0
t.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# t.frame = 0

# Frame Capture
fc.outputDir = '.'
fc.baseFilename = 'Mogwai'

