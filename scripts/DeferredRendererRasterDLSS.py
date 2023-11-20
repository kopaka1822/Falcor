from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DeferredRendererDLSS():
    g = RenderGraph('DeferredRendererDLSS')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Stratified', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('ShadowPass', 'ShadowPass', {})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'Balanced', 'motionVectorScale': 'Relative', 'isHDR': True, 'sharpness': 0.0, 'exposure': 0.0})
    g.add_edge('GBufferRaster.faceNormalW', 'ShadowPass.faceNormalW')
    g.add_edge('GBufferRaster.normW', 'ShadowPass.normalW')
    g.add_edge('GBufferRaster.tangentW', 'ShadowPass.tangentW')
    g.add_edge('GBufferRaster.texC', 'ShadowPass.texCoord')
    g.add_edge('GBufferRaster.mtlData', 'ShadowPass.MaterialInfor')
    g.add_edge('ShadowPass.color', 'DLSSPass.color')
    g.add_edge('GBufferRaster.depth', 'DLSSPass.depth')
    g.add_edge('GBufferRaster.mvec', 'DLSSPass.mvec')
    g.add_edge('GBufferRaster.posW', 'ShadowPass.posW')
    g.add_edge('DLSSPass.output', 'ToneMapper.src')
    g.mark_output('ToneMapper.dst')
    return g

DeferredRendererDLSS = render_graph_DeferredRendererDLSS()
try: m.addGraph(DeferredRendererDLSS)
except NameError: None