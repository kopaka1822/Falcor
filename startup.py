# Graphs
from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BillboardRayTracer.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ConstantColor.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    BillboardRayTracer = createPass('BillboardRayTracer')
    g.addPass(BillboardRayTracer, 'BillboardRayTracer')
    ToneMapper = createPass('ToneMapper', {'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapper, 'ToneMapper')
    g.addEdge('BillboardRayTracer.color', 'ToneMapper.src')
    g.markOutput('ToneMapper.dst')
    g.markOutput('BillboardRayTracer.color')
    return g
m.addGraph(render_graph_DefaultRenderGraph())

# Scene
m.loadScene('C:/Users/Felix/Documents/git/Falcor/BillboardScenes/test_area/test_area_spheres.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useVolumes=True)
m.scene.camera.position = float3(6.804514,2.521546,-2.763845)
m.scene.camera.target = float3(5.846595,2.311095,-2.568650)
m.scene.camera.up = float3(0.000000,1.000000,0.000000)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(1920, 1061)
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

