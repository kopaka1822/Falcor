# Graphs
from falcor import *

def render_graph_SSAO():
    g = RenderGraph('SSAO')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
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
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SSAO = createPass('SSAO', {'enabled': True, 'halfResolution': False, 'kernelSize': 16, 'noiseSize': uint2(16,16), 'radius': 2.0, 'distribution': SampleDistribution.UniformHammersley, 'blurWidth': 5, 'blurSigma': 2.0})
    g.addPass(SSAO, 'SSAO')
    NVIDIADenoiser = createPass('NVIDIADenoiser')
    g.addPass(NVIDIADenoiser, 'NVIDIADenoiser')
    ConvertFormat = createPass('ConvertFormat', {'formula': 'I0[xy]', 'format': ResourceFormat.R32Float})
    g.addPass(ConvertFormat, 'ConvertFormat')
    ConvertFormat_ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y,0,0,0) ', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat_, 'ConvertFormat_')
    ConvertFormat__ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].xyz * 0.5 + 0.5, 1.0)', 'format': ResourceFormat.RGBA8Unorm})
    g.addPass(ConvertFormat__, 'ConvertFormat__')
    ConvertFormat___ = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat___, 'ConvertFormat___')
    g.addEdge('ConvertFormat_.out', 'NVIDIADenoiser.input')
    g.addEdge('GBufferRaster.depth', 'SSAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.addEdge('GBufferRaster.mvec', 'NVIDIADenoiser.motion vector')
    g.addEdge('GBufferRaster.linearZ', 'ConvertFormat.I0')
    g.addEdge('ConvertFormat.out', 'NVIDIADenoiser.linear depth')
    g.addEdge('SSAO.ambientMap', 'ConvertFormat_.I0')
    g.addEdge('GBufferRaster.roughness', 'ConvertFormat__.I1')
    g.addEdge('ConvertFormat__.out', 'NVIDIADenoiser.normalRoughness')
    g.addEdge('NVIDIADenoiser.output', 'ConvertFormat___.I0')
    g.addEdge('GBufferRaster.faceNormalW', 'ConvertFormat__.I0')
    g.markOutput('SSAO.ambientMap')
    g.markOutput('ConvertFormat___.out')
    return g
m.addGraph(render_graph_SSAO())
from falcor import *

def render_graph_HBAOPlus():
    g = RenderGraph('HBAOPlus')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
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

# Scene
m.loadScene('D:/scenes/obj/sponza/sponza.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useVolumes=True)
m.scene.camera.position = float3(-13.956573,4.567351,3.201133)
m.scene.camera.target = float3(-14.588037,4.430812,3.964423)
m.scene.camera.up = float3(0.000621,1.000000,-0.000750)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(1920, 1017)
m.ui = True

# Clock Settings
m.clock.time = 0
m.clock.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# m.clock.frame = 0

# Frame Capture
m.frameCapture.outputDir = '.'
m.frameCapture.baseFilename = 'Mogwai'

