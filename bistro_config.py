# Graphs
from falcor import *

def render_graph_SSAO():
    g = RenderGraph('SSAO')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullNone})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SSAO = createPass('SSAO', {'enabled': True, 'kernelSize': 8, 'noiseSize': uint2(4,4), 'radius': 0.10000000149011612, 'distribution': SampleDistribution.Hammersley, 'depthMode': DepthMode.DualDepth})
    g.addPass(SSAO, 'SSAO')
    ConvertFormat_ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0) ', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat_, 'ConvertFormat_')
    ConvertFormat___ = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat___, 'ConvertFormat___')
    CrossBilateralBlur = createPass('CrossBilateralBlur')
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    DualDepthPass = createPass('DualDepthPass')
    g.addPass(DualDepthPass, 'DualDepthPass')
    LinearizeDepth = createPass('LinearizeDepth')
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    LinearizeDepth_ = createPass('LinearizeDepth')
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 8, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    g.addEdge('LinearizeDepth_.linearDepth', 'SSAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.addEdge('SSAO.ambientMap', 'ConvertFormat_.I0')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat___.I0')
    g.addEdge('DualDepthPass.depth', 'LinearizeDepth_.depth')
    g.addEdge('DualDepthPass.depth2', 'LinearizeDepth.depth')
    g.addEdge('LinearizeDepth.linearDepth', 'SSAO.depth2')
    g.addEdge('LinearizeDepth_.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('ConvertFormat_.out', 'CrossBilateralBlur.color')
    g.addEdge('DualDepthPass.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('StochasticDepthMap.linearSDepth', 'SSAO.stochasticDepth')
    g.markOutput('SSAO.ambientMap')
    g.markOutput('ConvertFormat___.out')
    return g
m.addGraph(render_graph_SSAO())
from falcor import *

def render_graph_HBAOPlus():
    g = RenderGraph('HBAOPlus')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
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
m.loadScene('D:/scenes/fbx/Bistro_v5_2/BistroExterior.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useVolumes=True)
m.scene.animated = False
m.scene.camera.animated = False
m.scene.camera.position = float3(-9.804805,3.688854,-5.694281)
m.scene.camera.target = float3(-8.934629,3.476815,-5.249496)
m.scene.camera.up = float3(-0.000765,1.000000,-0.000391)
m.scene.cameraSpeed = 1.0

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

