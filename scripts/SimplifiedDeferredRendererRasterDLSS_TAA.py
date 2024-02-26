from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DeferredRenderer():
    g = RenderGraph('DeferredRenderer')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'DirectX', 'sampleCount': 8, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('ShadowPass', 'ShadowPass', {})
    g.create_pass('VideoRecorder', 'VideoRecorder', {})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Relative', 'isHDR': False, 'sharpness': 0.0, 'exposure': 0.0})
    g.create_pass('PathBenchmark', 'PathBenchmark', {})
    g.add_edge('GBufferRaster.posW', 'ShadowPass.posW')
    g.add_edge('GBufferRaster.faceNormalW', 'ShadowPass.faceNormalW')
    g.add_edge('GBufferRaster.diffuseOpacity', 'ShadowPass.diffuse')
    g.add_edge('GBufferRaster.specRough', 'ShadowPass.specularRoughness')
    g.add_edge('GBufferRaster.emissive', 'ShadowPass.emissive')
    g.add_edge('GBufferRaster.guideNormalW', 'ShadowPass.guideNormalW')
    g.add_edge('ShadowPass.color', 'DLSSPass.color')
    g.add_edge('GBufferRaster.depth', 'DLSSPass.depth')
    g.add_edge('GBufferRaster.mvec', 'DLSSPass.mvec')
    g.add_edge('GBufferRaster.mvec', 'ShadowPass.motionVector')
    g.add_edge('VideoRecorder', 'GBufferRaster')
    g.add_edge('PathBenchmark', 'VideoRecorder')
    g.add_edge('GBufferRaster.mvec', 'ToneMapper.src')
    g.mark_output('DLSSPass.output')
    g.mark_output('ToneMapper.dst')
    return g

DeferredRenderer = render_graph_DeferredRenderer()
try: m.addGraph(DeferredRenderer)
except NameError: None
