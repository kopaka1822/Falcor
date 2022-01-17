# Graphs
from falcor import *

def render_graph_SSAO():
    g = RenderGraph('SSAO')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': True, 'cull': CullMode.CullNone})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SSAO = createPass('SSAO', {'enabled': True, 'halfResolution': False, 'kernelSize': 8, 'noiseSize': uint2(4,4), 'radius': 0.1, 'distribution': SampleDistribution.Hammersley})
    g.addPass(SSAO, 'SSAO')
    ConvertFormat = createPass('ConvertFormat', {'formula': 'I0[xy]', 'format': ResourceFormat.R32Float})
    g.addPass(ConvertFormat, 'ConvertFormat')
    ConvertFormat_ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0) ', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat_, 'ConvertFormat_')
    ConvertFormat___ = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat___, 'ConvertFormat___')
    CrossBilateralBlur = createPass('CrossBilateralBlur')
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.addEdge('GBufferRaster.linearZ', 'ConvertFormat.I0')
    g.addEdge('SSAO.ambientMap', 'ConvertFormat_.I0')
    g.addEdge('ConvertFormat.out', 'CrossBilateralBlur.linear depth')
    g.addEdge('ConvertFormat_.out', 'CrossBilateralBlur.color')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat___.I0')
    g.addEdge('ConvertFormat.out', 'SSAO.depth')
    g.markOutput('SSAO.ambientMap')
    g.markOutput('ConvertFormat___.out')
    return g
m.addGraph(render_graph_SSAO())
from falcor import *

def render_graph_HBAOPlus():
    g = RenderGraph('HBAOPlus')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    HBAOPlus = createPass('HBAOPlus')
    g.addPass(HBAOPlus, 'HBAOPlus')
    DualDepthPass = createPass('DualDepthPass')
    g.addPass(DualDepthPass, 'DualDepthPass')
    g.addEdge('DualDepthPass.depth', 'HBAOPlus.depth')
    g.addEdge('DualDepthPass.depth2', 'HBAOPlus.depth2')
    g.markOutput('HBAOPlus.ambientMap')
    return g
m.addGraph(render_graph_HBAOPlus())
from falcor import *

# Scene
m.loadScene('D:/scenes/fbx/Bistro_v5_2/BistroExterior.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useVolumes=True)
m.scene.cameraSpeed = 1.0
m.scene.camera.nearPlane = 0.2

# Window Configuration
m.resizeSwapChain(1920, 1137)
m.ui = True

# Clock Settings
m.clock.time = 0
m.clock.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# m.clock.frame = 0

# Frame Capture
m.frameCapture.outputDir = '.'
m.frameCapture.baseFilename = 'Mogwai'

