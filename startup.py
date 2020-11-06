# Graphs
from falcor import *

def render_graph_forward_renderer():
    g = RenderGraph('forward_renderer')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ConstantColor.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    DepthPrePass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float})
    g.addPass(DepthPrePass, 'DepthPrePass')
    LightingPass = createPass('ForwardLightingPass', {'sampleCount': 1, 'enableSuperSampling': False})
    g.addPass(LightingPass, 'LightingPass')
    BlitPass = createPass('BlitPass', {'filter': SamplerFilter.Linear})
    g.addPass(BlitPass, 'BlitPass')
    ToneMapping = createPass('ToneMapper', {'exposureCompensation': 0.0, 'autoExposure': False, 'exposureValue': 0.0, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapping, 'ToneMapping')
    SkyBox = createPass('SkyBox', {'texName': '', 'loadAsSrgb': True, 'filter': SamplerFilter.Linear})
    g.addPass(SkyBox, 'SkyBox')
    ConstantColor = createPass('ConstantColor')
    g.addPass(ConstantColor, 'ConstantColor')
    g.addEdge('DepthPrePass.depth', 'SkyBox.depth')
    g.addEdge('SkyBox.target', 'LightingPass.color')
    g.addEdge('DepthPrePass.depth', 'LightingPass.depth')
    g.addEdge('LightingPass.color', 'ToneMapping.src')
    g.addEdge('ToneMapping.dst', 'BlitPass.src')
    g.addEdge('ConstantColor.texture', 'LightingPass.visibilityBuffer')
    g.markOutput('BlitPass.dst')
    return g
m.addGraph(render_graph_forward_renderer())

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

