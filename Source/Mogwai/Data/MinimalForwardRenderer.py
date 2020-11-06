from falcor import *

def render_graph_forward_renderer():
    g = RenderGraph('forward_renderer')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ConstantColor.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    DepthPrePass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float})
    g.addPass(DepthPrePass, 'DepthPrePass')
    LightingPass = createPass('ForwardLightingPass', {'sampleCount': 1, 'enableSuperSampling': False})
    g.addPass(LightingPass, 'LightingPass')
    BlitPass = createPass('BlitPass', {'filter': SamplerFilter.Linear})
    g.addPass(BlitPass, 'BlitPass')
    ToneMapping = createPass('ToneMapper', {'exposureCompensation': 0.0, 'autoExposure': True, 'exposureValue': 0.0, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
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

forward_renderer = render_graph_forward_renderer()
try: m.addGraph(forward_renderer)
except NameError: None
