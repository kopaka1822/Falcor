from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ForwardDLSS():
    g = RenderGraph('ForwardDLSS')
    g.create_pass('GBufferRaster', 'GBufferRaster', {'outputSize': 'Default', 'samplePattern': 'Halton', 'sampleCount': 8, 'useAlphaTest': True, 'alphaTestMode': 'Basic', 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'textureLodBias': 0.0})
    g.create_pass('RayShadow', 'RayShadow', {'RayCones': False, 'DiminishBorder': False, 'RayConeShadow': 'Saturated', 'PointLightClip': 0.20000000298023224})
    g.create_pass('ForwardLighting', 'ForwardLighting', {'envMapIntensity': 0.25, 'ambientIntensity': 0.25, 'lightIntensity': 0.5, 'envMapMirror': False})
    g.create_pass('EnvMapPass', 'EnvMapPass', {})
    g.create_pass('DLSSPass', 'DLSSPass', {'enabled': True, 'outputSize': 'Default', 'profile': 'DLAA', 'preset': 'Default(CNN)', 'motionVectorScale': 'Relative', 'isHDR': True, 'useJitteredMV': False, 'sharpness': 0.3499999940395355, 'exposure': 0.0})
    g.add_edge('GBufferRaster.posW', 'RayShadow.posW')
    g.add_edge('GBufferRaster.normW', 'RayShadow.normalW')
    g.add_edge('GBufferRaster.depth', 'ForwardLighting.depth')
    g.add_edge('GBufferRaster.depth', 'EnvMapPass.depth')
    g.add_edge('EnvMapPass.color', 'ForwardLighting.color')
    g.add_edge('RayShadow.visibility', 'ForwardLighting.visibilityBuffer')
    g.add_edge('GBufferRaster.mvec', 'DLSSPass.mvec')
    g.add_edge('GBufferRaster.depth', 'DLSSPass.depth')
    g.add_edge('ForwardLighting.color', 'DLSSPass.color')
    g.mark_output('DLSSPass.output')
    return g

ForwardDLSS = render_graph_ForwardDLSS()
try: m.addGraph(ForwardDLSS)
except NameError: None
