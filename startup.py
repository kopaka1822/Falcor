# Graphs
from falcor import *

def render_graph_SSAO():
    g = RenderGraph('SSAO')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
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
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': True, 'cull': CullMode.CullNone})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SSAO = createPass('SSAO', {'enabled': True, 'halfResolution': False, 'kernelSize': 8, 'noiseSize': uint2(4,4), 'radius': 0.5, 'distribution': SampleDistribution.Hammersley})
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
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
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

def render_graph_SVFG_AO():
    g = RenderGraph('SVFG_AO')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
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
    SSAO = createPass('SSAO', {'enabled': True, 'halfResolution': False, 'kernelSize': 16, 'noiseSize': uint2(16,16), 'radius': 1.0, 'distribution': SampleDistribution.Hammersley})
    g.addPass(SSAO, 'SSAO')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    SVGFPass = createPass('SVGFPass', {'Enabled': True, 'Iterations': 4, 'FeedbackTap': 1, 'VarianceEpsilon': 9.999999747378752e-05, 'PhiColor': 10.0, 'PhiNormal': 128.0, 'Alpha': 0.05000000074505806, 'MomentsAlpha': 0.20000000298023224})
    g.addPass(SVGFPass, 'SVGFPass')
    ConvertFormat = createPass('ConvertFormat', {'formula': 'float4(I0[xy].r + I0[xy].g, I0[xy].r + I0[xy].g, I0[xy].r + I0[xy].g, 1.0)', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat, 'ConvertFormat')
    ConvertFormat_ = createPass('ConvertFormat', {'formula': '1.0', 'format': ResourceFormat.RGBA8Unorm})
    g.addPass(ConvertFormat_, 'ConvertFormat_')
    g.addEdge('GBufferRaster.depth', 'SSAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SSAO.normals')
    g.addEdge('GBufferRaster.posW', 'SVGFPass.WorldPosition')
    g.addEdge('GBufferRaster.normW', 'SVGFPass.WorldNormal')
    g.addEdge('GBufferRaster.pnFwidth', 'SVGFPass.PositionNormalFwidth')
    g.addEdge('GBufferRaster.linearZ', 'SVGFPass.LinearZ')
    g.addEdge('GBufferRaster.mvec', 'SVGFPass.MotionVec')
    g.addEdge('GBufferRaster.emissive', 'SVGFPass.Emission')
    g.addEdge('SSAO.ambientMap', 'ConvertFormat.I0')
    g.addEdge('ConvertFormat.out', 'SVGFPass.Color')
    g.addEdge('ConvertFormat_.out', 'SVGFPass.Albedo')
    g.markOutput('SVGFPass.Filtered image')
    g.markOutput('ConvertFormat.out')
    return g
m.addGraph(render_graph_SVFG_AO())

# Scene
m.loadScene('D:/scenes/obj/sponza/sponza.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useVolumes=True)
m.scene.camera.position = float3(-14.492852,6.504752,0.396478)
m.scene.camera.target = float3(-13.571704,6.355384,0.755887)
m.scene.camera.up = float3(-0.000810,1.000000,-0.000317)
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

