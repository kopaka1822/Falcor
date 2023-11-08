from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DeferredRenderer():
    g = RenderGraph('DeferredRenderer')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('ShadowPass', 'ShadowPass', {})
    g.add_edge('GBufferRaster.posW', 'ShadowPass.posW')
    g.add_edge('GBufferRaster.faceNormalW', 'ShadowPass.faceNormalW')
    g.add_edge('ShadowPass.color', 'ToneMapper.src')
    g.add_edge('GBufferRaster.normW', 'ShadowPass.normalW')
    g.add_edge('GBufferRaster.tangentW', 'ShadowPass.tangentW')
    g.add_edge('GBufferRaster.texC', 'ShadowPass.texCoord')
    g.add_edge('GBufferRaster.mtlData', 'ShadowPass.MaterialInfo')
    g.add_edge('GBufferRaster.texGrads', 'ShadowPass.texGrads')
    g.add_edge('GBufferRaster.diffuseOpacity', 'ShadowPass.diffuse')
    g.add_edge('GBufferRaster.specRough', 'ShadowPass.specularRoughness')
    g.mark_output('ToneMapper.dst')
    return g

DeferredRenderer = render_graph_DeferredRenderer()
try: m.addGraph(DeferredRenderer)
except NameError: None
