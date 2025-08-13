from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ForwardSMAA():
    g = RenderGraph('ForwardSMAA')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Decima2x', 'sampleCount': 2, 'useAlphaTest': True, 'alphaTestMode': 'Basic', 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'textureLodBias': 0.0})
    g.create_pass('RayShadow', 'RayShadow', {'RayCones': False, 'DiminishBorder': False, 'RayConeShadow': 'Saturated', 'PointLightClip': 0.20000000298023224})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Linear', 'clamp': False, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('ForwardLighting', 'ForwardLighting', {'envMapIntensity': 0.25, 'ambientIntensity': 0.25, 'lightIntensity': 0.5, 'envMapMirror': False})
    g.create_pass('EnvMapPass', 'EnvMapPass', {})
    g.create_pass('SMAA', 'SMAA', {})
    g.add_edge('GBufferRaster.posW', 'RayShadow.posW')
    g.add_edge('GBufferRaster.normW', 'RayShadow.normalW')
    g.add_edge('GBufferRaster.depth', 'ForwardLighting.depth')
    g.add_edge('GBufferRaster.depth', 'EnvMapPass.depth')
    g.add_edge('EnvMapPass.color', 'ForwardLighting.color')
    g.add_edge('ForwardLighting.color', 'ToneMapper.src')
    g.add_edge('RayShadow.visibility', 'ForwardLighting.visibilityBuffer')
    g.add_edge('ToneMapper.dst', 'SMAA.colorIn')
    g.add_edge('GBufferRaster.mvec', 'SMAA.mvec')
    g.add_edge('GBufferRaster.linearZ', 'SMAA.linearDepth')
    g.mark_output('SMAA.colorOut')
    return g

ForwardSMAA = render_graph_ForwardSMAA()
try: m.addGraph(ForwardSMAA)
except NameError: None
